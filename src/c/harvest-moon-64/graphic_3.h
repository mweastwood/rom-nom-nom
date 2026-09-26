#ifndef GRAPHIC_3_H
#define GRAPHIC_3_H

#include "graphic_model.h"
#include "graphic_plane.h"
#include "types.h"

extern const f64 g_degrees_to_radians_direction;

s32 PlaneCheckNormals(f32 x0, f32 y0, f32 z0, f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2,
                      f32 x3, f32 y3, f32 z3);
f32 PlaneEvaluate(f32 x, f32 y, f32 z, Plane plane);
Vec3f* GraphicRotateDirectionVector(Vec3f* out, f32 z, u8 direction, f32 y);

// Libultra math routines
f32 Sinf(f32 angle);
f32 Cosf(f32 angle);

#endif
