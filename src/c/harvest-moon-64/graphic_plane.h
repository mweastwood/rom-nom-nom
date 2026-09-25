#ifndef GRAPHIC_PLANE_H
#define GRAPHIC_PLANE_H

#include "types.h"

typedef struct {
  f32 a;
  f32 c;
  f32 b;
  f32 d;
} Plane;

f32 PlaneCalculateZ(f32 x, f32 y, Plane plane);

#endif
