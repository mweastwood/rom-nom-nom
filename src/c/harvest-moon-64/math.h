#ifndef HARVEST_MOON_64_MATH_H
#define HARVEST_MOON_64_MATH_H

#include "types.h"

typedef struct {
  s16 step;
  s16 fraction;
  s16 accumulator;
  s32 current;
  s32 target;
} Interpolator;

void InterpolateInit(Interpolator* interp, s32 current, s32 target, s16 step, s16 fraction);
s32 InterpolateUpdate(Interpolator* interp);
void InterpolateStepInit(Interpolator* interp, s16 step, s16 fraction);
s16 InterpolateStep(Interpolator* interp);
void InterpolateSetStep(Interpolator* interp, s16 accumulator);
s16 InterpolateGetStep(Interpolator* interp);
s16 Abs16(s16 val);
s32 RandomRange(s32 min, s32 max);

#endif
