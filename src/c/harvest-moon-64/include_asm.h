#ifndef HARVEST_MOON_64_INCLUDE_ASM_H
#define HARVEST_MOON_64_INCLUDE_ASM_H

#if !defined(M2CTX) && !defined(PERMUTER) && !defined(__clang__) && !defined(MODERN_TOOLCHAIN)

#ifndef INCLUDE_ASM
#define INCLUDE_ASM(FOLDER, NAME)        \
  __asm__(                               \
      ".section .text\n"                 \
      "    .set noat\n"                  \
      "    .set noreorder\n"             \
      "    .include \"" FOLDER "/" #NAME \
      ".s\"\n"                           \
      "    .set reorder\n"               \
      "    .set at\n")
#endif

#ifndef INCLUDE_RODATA
#define INCLUDE_RODATA(FOLDER, NAME)     \
  __asm__(                               \
      ".section .rodata\n"               \
      "    .include \"" FOLDER "/" #NAME \
      ".s\"\n"                           \
      ".section .text")
#endif

#else

#ifndef INCLUDE_ASM
#define INCLUDE_ASM(FOLDER, NAME)
#endif
#ifndef INCLUDE_RODATA
#define INCLUDE_RODATA(FOLDER, NAME)
#endif

#endif

#endif
