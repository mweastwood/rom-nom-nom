#ifndef GRAPHIC_TRIG_H
#define GRAPHIC_TRIG_H

#include "types.h"

extern const f64 g_degrees_to_radians;
extern const f64 g_degrees_to_radians_cos;

f32 SinDegrees(f32 angle);
f32 CosDegrees(f32 angle);

// Libultra math routines
f32 __sinf(f32 angle);
f32 func_800FF130(f32 angle);

#endif
