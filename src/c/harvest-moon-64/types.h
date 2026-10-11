#ifndef HARVEST_MOON_64_TYPES_H
#define HARVEST_MOON_64_TYPES_H

typedef signed char s8;
typedef unsigned char u8;
typedef signed short s16;
typedef unsigned short u16;
typedef signed int s32;
typedef unsigned int u32;
typedef signed long long s64;
typedef unsigned long long u64;

typedef float f32;
typedef double f64;

#ifndef NULL
#define NULL 0
#endif

#if defined(__GNUC__) && (defined(__mips__) || defined(__mips))
#define NOP() __asm__ volatile("nop")
static inline f32 N64Sqrtf(f32 x) {
  f32 res;
  __asm__("sqrt.s %0, %1" : "=f"(res) : "f"(x));
  return res;
}
static inline f64 N64Sqrt(f64 x) {
  f64 res;
  __asm__("sqrt.d %0, %1" : "=f"(res) : "f"(x));
  return res;
}
#else
#define NOP() ((void)0)
#ifdef __cplusplus
#include <cmath>
static inline f32 N64Sqrtf(f32 x) {
  return std::sqrt(x);
}
static inline f64 N64Sqrt(f64 x) {
  return std::sqrt(x);
}
#else
#include <math.h>
static inline f32 N64Sqrtf(f32 x) {
  return sqrtf(x);
}
static inline f64 N64Sqrt(f64 x) {
  return sqrt(x);
}
#endif
#endif

#define __n64_sqrtf N64Sqrtf
#define __n64_sqrt N64Sqrt

typedef s32 OSId;
typedef s32 OSPri;

typedef struct OSThread_s {
  struct OSThread_s* next;
  OSPri priority;
  struct OSThread_s** queue;
  struct OSThread_s* tlnext;
  u16 state;
  u16 flags;
  OSId id;
  s32 fp;
} OSThread;

typedef void* OSMesg;

typedef struct {
  OSMesg* msg;
  s32 msg_count;
  s32 first;
  s32 valid_count;
} OSMesgQueue;

typedef long int Mtx_t[4][4];
typedef union {
  Mtx_t m;
  long long int force_structure_alignment;
} Mtx;

typedef union {
  struct {
    u32 w0;
    u32 w1;
  } words;
  s64 force_structure_alignment;
} Gfx;

typedef struct {
  u8 pad[16];
} Light;

typedef struct {
  s16 ob[3];
  u16 flag;
  s16 tc[2];
  u8 cn[4];
} VtxT;

typedef union {
  VtxT v;
  s64 force_structure_alignment;
} Vtx;

typedef union {
  struct {
    Light l[2];
  } l;
  s64 force_structure_alignment[4];
} LookAt;

typedef struct {
  f32 x;
  f32 y;
  f32 z;
} Vec3f;

typedef struct {
  f32 x;
  f32 y;
  f32 z;
  f32 w;
} Vec4f;

#endif
