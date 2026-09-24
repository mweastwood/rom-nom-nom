#include "math.h"

s32 func_80110410(void);

void InterpolateInit(Interpolator* interp, s32 current, s32 target, s16 step, s16 fraction) {
  interp->current = current;
  interp->target = target;
  InterpolateStepInit(interp, step, fraction);
}

s32 InterpolateUpdate(Interpolator* interp) {
  s32 ret = 0;
  s32 step = (s16) + InterpolateStep(interp);

  if (interp->current < interp->target) {
    interp->current += step;
    if (interp->current >= interp->target) {
      interp->step = 0;
      interp->current = interp->target;
    } else {
      ret = 1;
    }
  }
  if (interp->current > interp->target) {
    interp->current -= step;
    if (interp->target >= interp->current) {
      interp->step = 0;
      interp->current = interp->target;
    } else {
      ret = 1;
    }
  }
  return ret;
}

void InterpolateStepInit(Interpolator* interp, s16 step, s16 fraction) {
  interp->step = step;
  interp->fraction = fraction;
  interp->accumulator = 0;
}

s16 InterpolateStep(Interpolator* interp) {
  s16 step;

  if (interp->step < 0) {
    step = (interp->fraction % (~interp->step + 1)) == 0;
    interp->fraction++;
  } else {
    step = interp->step;
  }
  interp->accumulator += step;
  return step;
}

void InterpolateSetStep(Interpolator* interp, s16 accumulator) {
  interp->accumulator = accumulator;
}

s16 InterpolateGetStep(Interpolator* interp) {
  return interp->accumulator;
}

s16 Abs16(s16 val) {
  if (val < 0) {
    return -val;
  }
  return val;
}

s32 RandomRange(s32 min, s32 max) {
  s32 rand;
  u16 temp;
  func_80110410();
  max &= 0xFFFF;
  temp = min;
  rand = func_80110410() & 0xFFFF;
  return (min + (rand * (max - temp + 1) / 32768)) & 0xFFFF;
}
