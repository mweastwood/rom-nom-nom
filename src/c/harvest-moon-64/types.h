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

#endif
