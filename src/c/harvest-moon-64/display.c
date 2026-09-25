#include "display.h"

void RenderInit(void) {
  s32 i = 0;

  g_display_matrices[0] = 0xFF;
  g_display_matrices[1] = 0xFF;
  g_display_matrices[2] = 0xFF;
  g_display_matrices[3] = 0xFF;
  g_display_matrices[4] = 0xFF;
  g_display_matrices[5] = 0xFF;
  g_display_matrices[6] = 0xFF;
  g_display_matrices[7] = 0xFF;
  g_display_matrices[8] = 0xFF;

  do {
    g_render_entries[i & 0xFFFF].flags = 0;
    g_render_entries[i & 0xFFFF].translation.x = 0;
    g_render_entries[i & 0xFFFF].translation.y = 0;
    g_render_entries[i & 0xFFFF].translation.z = 0;
    g_render_entries[i & 0xFFFF].scale.x = 0;
    g_render_entries[i & 0xFFFF].scale.y = 0;
    g_render_entries[i & 0xFFFF].scale.z = 0;
    g_render_entries[i & 0xFFFF].rotation.x = 0;
    g_render_entries[i & 0xFFFF].rotation.y = 0;
    g_render_entries[i & 0xFFFF].rotation.z = 0;
    i++;
  } while ((u32)(i & 0xFFFF) < 336);

  RenderResetCount();
}

void RenderResetCount(void) {
  g_render_entry_count = 0;
}

void RenderNoOp(void) {}

void RenderAddEntry(u32 dl, u16 flags) {
#ifdef __clang__
  register u32 index = g_render_entry_count;
#else
  register u32 index asm("$2") = g_render_entry_count;
#endif
  u16 next = index + 1;
  index &= 0xFFFF;
  g_render_entries[index].dl = dl;
  g_render_entries[index].flags = flags | 1;
  g_render_entry_count = next;
}

s32 RenderSetTranslation(s32 index, f32 x, f32 y, f32 z) {
  u32 idx = index & 0xFFFF;
  g_render_entries[idx].translation.x = x;
  g_render_entries[idx].translation.y = y;
  g_render_entries[idx].translation.z = z;
  return 1;
}

s32 RenderSetRotation(s32 index, f32 x, f32 y, f32 z) {
  u32 idx = index & 0xFFFF;
  g_render_entries[idx].scale.x = x;
  g_render_entries[idx].scale.y = y;
  g_render_entries[idx].scale.z = z;
  return 1;
}

s32 RenderSetScale(s32 index, f32 x, f32 y, f32 z) {
  u32 idx = index & 0xFFFF;
  g_render_entries[idx].rotation.x = x;
  g_render_entries[idx].rotation.y = y;
  g_render_entries[idx].rotation.z = z;
  return 1;
}

void RenderNoOp2(void) {}

#define OS_K0_TO_PHYSICAL(x) ((u32)(x)-0x80000000)

#define gDPPipeSync(pkt)       \
  {                            \
    Gfx* _g = (pkt);           \
    _g->words.w0 = 0xE7000000; \
    _g->words.w1 = 0;          \
  }

#define gSPMatrix(pkt, m, p)             \
  {                                      \
    Gfx* _g = (pkt);                     \
    _g->words.w0 = (0xDA380000 | (p));   \
    _g->words.w1 = OS_K0_TO_PHYSICAL(m); \
  }

#define gSPDisplayList(pkt, dl)           \
  {                                       \
    Gfx* _g = (pkt);                      \
    _g->words.w0 = 0xDE000000;            \
    _g->words.w1 = OS_K0_TO_PHYSICAL(dl); \
  }

#define GFX_CMD(pkt, cmd0, cmd1) \
  {                              \
    Gfx* _g = (pkt);             \
    _g->words.w0 = (cmd0);       \
    _g->words.w1 = (cmd1);       \
  }

Gfx* RenderDrawEntries(Gfx* dl, CameraContext* camera) {
  Vec3f pos;
  f32 pad[12];
  u16 i;
  f32 sin_yaw;
  f32 sin_pitch;
  f32 cos_yaw;
  f32 cos_pitch;

  gDPPipeSync(dl++);
  GFX_CMD(dl++, 0xE3000A01, 0);
  GFX_CMD(dl++, 0xE3001001, 0x8000);
  GFX_CMD(dl++, 0xE2001E01, 0);
  GFX_CMD(dl++, 0xE2001D00, 0);
  GFX_CMD(dl++, 0xD9000000, 0);
  GFX_CMD(dl++, 0xD9FFFFFF, 0x200205);
  GFX_CMD(dl++, 0xDB040004, 6);
  GFX_CMD(dl++, 0xDB04000C, 6);
  GFX_CMD(dl++, 0xDB040014, 0xFFFA);
  GFX_CMD(dl++, 0xDB04001C, 0xFFFA);
  GFX_CMD(dl++, 0xD7000002, 0x80008000);

  guTranslate(&camera->translation_matrix, camera->translation[0], camera->translation[1],
              camera->translation[2]);
  guScale(&camera->scale_matrix, camera->scale[0], camera->scale[1], camera->scale[2]);
  guRotate(&camera->rotation_matrix_x, camera->rotation[0], 0.0f, 0.0f);
  guRotate(&camera->rotation_matrix_y, 0.0f, camera->rotation[1], 0.0f);
  guRotate(&camera->rotation_matrix_z, 0.0f, 0.0f, camera->rotation[2]);

  gSPMatrix(dl++, &camera->translation_matrix, 5);
  gSPMatrix(dl++, &camera->scale_matrix, 5);
  gSPMatrix(dl++, &camera->rotation_matrix_x, 5);
  gSPMatrix(dl++, &camera->rotation_matrix_y, 5);
  gSPMatrix(dl++, &camera->rotation_matrix_z, 5);

  if (g_camera_angles[0] < 0.0f) {
    g_camera_angles[0] += 360.0f;
  }
  if (g_camera_angles[0] >= 360.0f) {
    g_camera_angles[0] -= 360.0f;
  }
  if (g_camera_angles[1] < 0.0f) {
    g_camera_angles[1] += 360.0f;
  }
  if (g_camera_angles[1] >= 360.0f) {
    g_camera_angles[1] -= 360.0f;
  }

  sin_yaw = SinDegrees(g_camera_angles[0]);
  sin_pitch = SinDegrees(g_camera_angles[1]);
  cos_yaw = CosDegrees(g_camera_angles[0]);
  cos_pitch = CosDegrees(g_camera_angles[1]);
  SinDegrees(g_camera_angles_prev[0]);
  SinDegrees(g_camera_angles_prev[1]);
  CosDegrees(g_camera_angles_prev[0]);
  CosDegrees(g_camera_angles_prev[1]);

  i = g_render_entry_count;
  while (i < 336) {
    g_render_entries[i].flags = 0;
    i++;
  }

  i = 0;
  do {
    if (g_render_entries[i].flags & 1) {
      if (g_render_entries[i].flags & 0x40) {
        pos = g_render_entries[i].translation;
      } else {
        f32 z;
        f32 temp_x;
        f32 x_sin;
        f32 x_cos;
        f32 y_sin;
        f32 y_cos;
        f32 temp_y;

        z = g_render_entries[i].translation.z;
        temp_x = z * sin_pitch;
        z *= cos_pitch;

        x_sin = g_render_entries[i].translation.x * sin_pitch;
        x_cos = g_render_entries[i].translation.x * cos_pitch;
        z -= x_sin;

        y_sin = g_render_entries[i].translation.y * sin_yaw;
        y_cos = g_render_entries[i].translation.y * cos_yaw;

        temp_y = -z;
        z *= cos_yaw;
        pos.x = temp_x + x_cos;
        temp_y *= sin_yaw;
        pos.z = z + y_sin;
        pos.y = temp_y + y_cos;
      }

      if (g_render_entries[i].flags & 0x80) {
        f32 z = pos.z + 24.0f;
        f32 x = pos.x;
        f32 y = pos.y;
        guTranslate(&g_render_entries[i].translation_matrix, x, y, z);
      } else {
        guTranslate(&g_render_entries[i].translation_matrix, pos.x, pos.y, pos.z);
      }
      gSPMatrix(dl++, &g_render_entries[i].translation_matrix, 3);

      if (g_render_entries[i].flags & 0x20) {
        guRotate(&g_render_entries[i].rotation_matrix_x, g_render_entries[i].rotation.x, 0.0f,
                 0.0f);
        guRotate(&g_render_entries[i].rotation_matrix_y, 0.0f, g_render_entries[i].rotation.y,
                 0.0f);
        guRotate(&g_render_entries[i].rotation_matrix_z, 0.0f, 0.0f,
                 g_render_entries[i].rotation.z);
        gSPMatrix(dl++, &g_render_entries[i].rotation_matrix_x, 1);
        gSPMatrix(dl++, &g_render_entries[i].rotation_matrix_y, 1);
        gSPMatrix(dl++, &g_render_entries[i].rotation_matrix_z, 1);
      }

      if (g_render_entries[i].flags & 0x10) {
        guScale(&g_render_entries[i].scale_matrix, g_render_entries[i].scale.x,
                g_render_entries[i].scale.y, g_render_entries[i].scale.z);
        gSPMatrix(dl++, &g_render_entries[i].scale_matrix, 1);
      }

      gSPDisplayList(dl++, (Gfx*)g_render_entries[i].dl);
      g_render_entries[i].flags = 0;
    }
    i++;
  } while (i < 336);

  return dl;
}
