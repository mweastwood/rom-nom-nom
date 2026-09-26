#ifndef GRAPHIC_2_H
#define GRAPHIC_2_H

#include "types.h"

extern const f64 g_degrees_to_radians_euler;

void GraphicRotateEuler(Vec3f in, Vec3f* out, f32 angle_x, f32 angle_y, f32 angle_z);
void GraphicRotateVector(Vec3f in, Vec3f* out, Vec3f sin, Vec3f cos);
Vec4f* PlaneCalculateNormal(Vec4f* plane, f32 p1_x, f32 p1_y, f32 p1_z, f32 p2_x, f32 p2_y,
                            f32 p2_z, f32 p3_x, f32 p3_y, f32 p3_z);

f32 Sinf(f32 angle);
f32 Cosf(f32 angle);

#endif
