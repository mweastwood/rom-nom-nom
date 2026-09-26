#include "graphic_trig.h"

f32 SinDegrees(f32 angle) {
  return Cosf((f64)angle * g_degrees_to_radians);
}

f32 CosDegrees(f32 angle) {
  return Sinf((f64)angle * g_degrees_to_radians_cos);
}
