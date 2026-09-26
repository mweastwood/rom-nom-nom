#include "graphic_3.h"

s32 PlaneCheckNormals(f32 x0, f32 y0, f32 z0, f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2,
                      f32 x3, f32 y3, f32 z3) {
  Plane plane;
  Plane res;
  f32 nx;
  f32 ny;
  f32 nz;
  f32 sq;
  f32 len;
  s32 count = 0;

  // Triangle 1: (v0, v1, v2)
  nx = (y1 * (z2 - z0)) + (y2 * (z0 - z1)) + (y0 * (z1 - z2));
  ny = (z1 * (x2 - x0)) + (z2 * (x0 - x1)) + (z0 * (x1 - x2));
  nz = (x1 * (y2 - y0)) + (x2 * (y0 - y1)) + (x0 * (y1 - y2));

  sq = (nx * nx) + (ny * ny) + (nz * nz);
  len = sqrtf(sq);

  if (len != 0.0f) {
    res.a = nx / len;
    res.b = ny / len;
    res.c = nz / len;
    res.d = -((res.a * x1) + (res.b * y1) + (res.c * z1));
  } else {
    res.a = 0.0f;
    res.b = 0.0f;
    res.c = 0.0f;
    res.d = 0.0f;
  }
  plane = res;
  if (plane.b >= 0.0f) {
    count++;
  }

  // Triangle 2: (v0, v2, v3)
  nx = (y2 * (z3 - z0)) + (y3 * (z0 - z2)) + (y0 * (z2 - z3));
  ny = (z2 * (x3 - x0)) + (z3 * (x0 - x2)) + (z0 * (x2 - x3));
  nz = (x2 * (y3 - y0)) + (x3 * (y0 - y2)) + (x0 * (y2 - y3));

  sq = (nx * nx) + (ny * ny) + (nz * nz);
  len = sqrtf(sq);

  if (len != 0.0f) {
    res.a = nx / len;
    res.b = ny / len;
    res.c = nz / len;
    res.d = -((res.a * x2) + (res.b * y2) + (res.c * z2));
  } else {
    res.a = 0.0f;
    res.b = 0.0f;
    res.c = 0.0f;
    res.d = 0.0f;
  }
  plane = res;
  if (plane.b >= 0.0f) {
    count++;
  }

  // Triangle 3: (v0, v3, v1)
  nx = (y3 * (z1 - z0)) + (y1 * (z0 - z3)) + (y0 * (z3 - z1));
  ny = (z3 * (x1 - x0)) + (z1 * (x0 - x3)) + (z0 * (x3 - x1));
  nz = (x3 * (y1 - y0)) + (x1 * (y0 - y3)) + (x0 * (y3 - y1));

  sq = (nx * nx) + (ny * ny) + (nz * nz);
  len = sqrtf(sq);

  if (len != 0.0f) {
    res.a = nx / len;
    res.b = ny / len;
    res.c = nz / len;
    res.d = -((res.a * x3) + (res.b * y3) + (res.c * z3));
  } else {
    res.a = 0.0f;
    res.b = 0.0f;
    res.c = 0.0f;
    res.d = 0.0f;
  }
  plane = res;
  if (plane.b >= 0.0f) {
    count++;
  }

  return count / 3;
}

f32 PlaneEvaluate(f32 x, f32 y, f32 z, Plane plane) {
  return (plane.a * x) + (plane.b * y) + (plane.c * z) + plane.d;
}

Vec3f* GraphicRotateDirectionVector(Vec3f* out, f32 z, u8 direction, f32 y) {
  DirectionAngleTable angles = g_direction_angles;
  u8 dir = direction;
  Vec3f v;
  Vec3f res;
  Vec3f rot;
  Vec3f v_in;
  Vec3f rot_in;
  f32 rad_x;
  f32 rad_y;
  f32 rad_z;
  f32 cos_x;
  f32 sin_x;
  f32 cos_y;
  f32 sin_y;
  f32 cos_z;
  f32 sin_z;
  Vec3f temp;

  if (dir != 0xFF) {
    v.x = 0.0f;
    v.y = y;
    v.z = z;

    rot.x = 0.0f;
    rot.y = angles.angles[dir];
    rot.z = 0.0f;

    v_in = v;
    rot_in = rot;

    rad_x = (f64)rot_in.x * g_degrees_to_radians_direction;
    rad_y = (f64)rot_in.y * g_degrees_to_radians_direction;
    rad_z = (f64)rot_in.z * g_degrees_to_radians_direction;

    cos_x = Cosf(rad_x);
    sin_x = Sinf(rad_x);
    cos_y = Cosf(rad_y);
    sin_y = Sinf(rad_y);
    cos_z = Cosf(rad_z);
    sin_z = Sinf(rad_z);

    res = v_in;

    if (rad_z != 0.0f) {
      temp = v_in;
      temp.y = (v_in.y * cos_z) + (v_in.x * cos_z);
      temp.x = (-v_in.y * cos_z) + (v_in.x * sin_z);
      res = temp;
    }

    if (rad_y != 0.0f) {
      temp = res;
      temp.x = (res.z * cos_y) + (res.x * sin_y);
      temp.z = (res.z * sin_y) - (res.x * cos_y);
      res = temp;
    }

    if (rad_x != 0.0f) {
      temp = res;
      temp.z = (res.z * sin_x) + (res.y * cos_x);
      temp.y = (-res.z * cos_x) + (res.y * sin_x);
      res = temp;
    }
  } else {
    res.x = 0.0f;
    res.y = y;
    res.z = 0.0f;
  }

  *out = res;
  return out;
}
