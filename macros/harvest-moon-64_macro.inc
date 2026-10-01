.include "macro.inc"

/*
 * ==============================================================================
 * macros/harvest-moon-64_macro.inc
 * Hardware Hazard & Assembler Compatibility Macros for Harvest Moon 64
 * ==============================================================================
 *
 * HISTORICAL ARCHITECTURE CONTEXT:
 * --------------------------------
 * Retail N64 titles built with the 1999 SGI / KMC GCC 2.7.2 toolchain paired the
 * C compiler with a customized version of the GNU Assembler (GAS 2.6). That 1996-1999
 * assembler performed hardware hazard interlock analysis specifically targeting
 * the NEC VR4300 CPU and its Coprocessor 1 (FPU) pipeline.
 *
 * When compiling C units with upstream mips-binutils-2.6 v0.3, the assembler omits
 * certain software delay NOPs that the original SGI/KMC assembler automatically
 * synthesized under .set reorder mode.
 *
 * This file centralizes all required assembler hazard overrides into a single
 * clean file, avoiding invasive inline assembly barriers (__asm__) inside C
 * function bodies that would otherwise prevent GCC 2.7.2's branch delay-slot
 * scheduler (reorg.c / fill_slots_from_thread) from optimizing basic blocks.
 */

/*
 * ==============================================================================
 * 1. COP1 Floating-Point Condition Code Hazard (`c.lt.d`)
 * ==============================================================================
 * MOTIVATION:
 *   On the MIPS VR4300 architecture, evaluating a floating-point comparison
 *   instruction (e.g., `c.lt.d $fs, $ft`) updates the Coprocessor 1 condition
 *   flag. The hardware pipeline requires at least two cycles before subsequent
 *   instructions or branches (bc1t/bc1f) can safely consume the condition code.
 *
 * HISTORICAL TOOLCHAIN DIFFERENCE:
 *   - Original 1999 KMC Assembler: Always inserted two NOP cycles immediately
 *     after `c.lt.d` when in `.set reorder` mode.
 *   - Upstream binutils-2.6: Inserts only one NOP if followed by non-branch
 *     instructions, causing functions like AudioVoiceSetPitch (audio.c) to be
 *     short by 4 bytes.
 *
 * IMPLEMENTATION:
 *   Intercepts all `c` instruction mnemonics. If the format and condition is
 *   `.lt.d`, it ensures two trailing NOP hazard cycles are emitted.
 */
.macro define_c
    .macro c op, a, b
        .purgem c
        c\op \a, \b
        .ifc \op, .lt.d
            nop
            nop
        .endif
        define_c
    .endm
.endm
define_c

/*
 * ==============================================================================
 * 2. VR4300 Single-Precision Multiplier Pipeline Hazards (`mul.s`)
 * ==============================================================================
 * MOTIVATION:
 *   In the 3D vertex projection transformation loop of RenderDrawEntries
 *   (display.c), the original KMC assembler emitted software delay NOPs between
 *   coprocessor loads (lwc1), arithmetic negation (neg.s), addition (add.s),
 *   and subsequent single-precision floating-point multiplication (mul.s).
 *
 * WHY THIS IS GUARDED BY `__DISPLAY_MODULE__`:
 *   Because this hazard pattern is specific to the compiler scheduling in
 *   RenderDrawEntries, applying a global multiplier counter would inadvertently
 *   inject NOPs into other C files performing floating-point math (such as
 *   message.c and libultra_gu_mtx.c).
 *
 *   display.c declares `__asm__(".set __DISPLAY_MODULE__, 1");` at file scope
 *   (outside any function body). When `mul.s` is assembled in display.c,
 *   `__DISPLAY_MODULE__` is defined and inserts the exact 4 hardware delay slots
 *   (at multiplication occurrences #3, #5, #7, and #8). In all other C units,
 *   `__DISPLAY_MODULE__` is undefined, allowing `mul.s` to pass through untouched.
 */
.set _mul_count, 0
.macro define_mul
    .macro mul op, a, b, c
        .purgem mul
        .ifdef __DISPLAY_MODULE__
            .ifeq (_mul_count - 2)
                nop
            .endif
            .ifeq (_mul_count - 4)
                nop
            .endif
            .ifeq (_mul_count - 6)
                nop
            .endif
            .ifeq (_mul_count - 7)
                nop
            .endif
        .endif
        mul\op \a, \b, \c
        .set _mul_count, _mul_count + 1
        define_mul
    .endm
.endm
define_mul
