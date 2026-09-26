#include "graphic_2.h"

void GraphicRotateEuler(Vec3f in, Vec3f* out, f32 angle_x, f32 angle_y, f32 angle_z) {
  f32 temp_fa0;
  f32 temp_fa0_2;
  f32 temp_fs0;
  f32 temp_fs1;
  f32 temp_fs2;
  f32 temp_fs3;
  f32 temp_fs4;
  f32 temp_fs5;
  f32 temp_ft2;
  f32 temp_ft2_2;
  f32 temp_ft3;
  f32 temp_ft3_2;
  f32 temp_fv0;
  f32 temp_s1;
  f32 temp_s2;

  temp_fs3 = (f32)((f64)angle_x * g_degrees_to_radians_euler);
  temp_fs2 = (f32)((f64)angle_y * g_degrees_to_radians_euler);
  temp_fs0 = (f32)((f64)angle_z * g_degrees_to_radians_euler);
  temp_s2 = Cosf(temp_fs3);
  temp_s1 = Sinf(temp_fs3);
  temp_fs5 = Cosf(temp_fs2);
  temp_fs4 = Sinf(temp_fs2);
  temp_fs1 = Cosf(temp_fs0);
  temp_fv0 = Sinf(temp_fs0);
  out->x = in.x;
  temp_fa0 = out->x;
  out->y = in.y;
  temp_ft3 = out->y;
  out->z = in.z;
  if (temp_fs0 != 0.0f) {
    out->z = in.z;
    out->y = (temp_ft3 * temp_fs1) + (temp_fa0 * temp_fs1);
    out->x = (-temp_ft3 * temp_fs1) + (temp_fa0 * temp_fv0);
  }
  temp_fa0_2 = out->x;
  temp_ft2 = out->z;
  if (temp_fs2 != 0.0f) {
    out->y = out->y;
    out->x = (temp_ft2 * temp_fs5) + (temp_fa0_2 * temp_fs4);
    out->z = (temp_ft2 * temp_fs4) - (temp_fa0_2 * temp_fs5);
  }
  temp_ft3_2 = out->y;
  temp_ft2_2 = out->z;
  if (temp_fs3 != 0.0f) {
    out->x = out->x;
    out->z = (temp_ft2_2 * temp_s1) + (temp_ft3_2 * temp_s2);
    out->y = (-temp_ft2_2 * temp_s2) + (temp_ft3_2 * temp_s1);
  }
}

void GraphicRotateVector(Vec3f in, Vec3f* out, Vec3f sin, Vec3f cos) {
  f32 rx = (in.z * sin.y) + (in.x * cos.y);
  f32 temp = (in.z * cos.y) - (in.x * sin.y);
  f32 rz = (temp * cos.x) + (in.y * sin.x);
  f32 ry = (-temp * sin.x) + (in.y * cos.x);

  out->x = rx;
  out->z = rz;
  out->y = ry;
}

Vec4f* PlaneCalculateNormal(Vec4f* plane, f32 p1_x, f32 p1_y, f32 p1_z, f32 p2_x, f32 p2_y,
                            f32 p2_z, f32 p3_x, f32 p3_y, f32 p3_z) {
  Vec4f res;
  f32 nx;
  f32 ny;
  f32 nz;
  f32 sq;
  f32 len;

  nx = (p1_y * (p2_z - p3_z)) + (p2_y * (p3_z - p1_z)) + (p3_y * (p1_z - p2_z));
  ny = (p1_z * (p2_x - p3_x)) + (p2_z * (p3_x - p1_x)) + (p3_z * (p1_x - p2_x));
  nz = (p1_x * (p2_y - p3_y)) + (p2_x * (p3_y - p1_y)) + (p3_x * (p1_y - p2_y));

  sq = (nx * nx) + (ny * ny) + (nz * nz);
  len = sqrtf(sq);

  if (len == 0.0f) {
    res.x = 0.0f;
    res.y = 0.0f;
    res.z = 0.0f;
    res.w = 0.0f;
  } else {
    nx /= len;
    ny /= len;
    nz /= len;
    res.x = nx;
    res.y = ny;
    res.z = nz;
    res.w = -((nx * p1_x) + (ny * p1_y) + (nz * p1_z));
  }

  *plane = res;
  return plane;
}
