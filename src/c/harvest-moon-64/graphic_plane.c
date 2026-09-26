#include "graphic_plane.h"

f32 PlaneCalculateZ(f32 x, f32 y, Plane plane) {
  f32 z = 0.0f;

  if (plane.b != 0.0f) {
    z = ((-(plane.a * x) - (plane.c * y)) - plane.d) / plane.b;
  }
  return z;
}
