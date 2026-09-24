/*
 * gas_compat.inc — GNU Assembler (GAS) Compatibility Macros for N64 Decompilation
 *
 * HISTORICAL CONTEXT & ARCHITECTURAL MOTIVATION:
 * ----------------------------------------------
 * During the Nintendo 64 commercial era (1996-2001), retail software was compiled
 * with Silicon Graphics (SGI) / KMC GCC 2.7.2 paired with a 1996-era Cygnus/SGI
 * customized version of the GNU Assembler (`as`).
 *
 * In modern clean-room decompilation pipelines, we run the authentic GCC 2.7.2
 * compiler binary, but assemble its emitted intermediate assembly using modern
 * GNU Binutils `mips-linux-gnu-as` (version 2.40+).
 *
 * Modern GAS diverges from the 1996 SGI/Cygnus assembler in several critical ways:
 *
 * 1. Macro Expansion Ordering:
 *    Synthetic/pseudo-instructions (such as `remu` unsigned remainder, `div` signed
 *    division, and `move`) have different instruction scheduling and register usage
 *    in modern GAS compared to the 1996 assembler.
 *
 * 2. MIPS I/II Pipeline & Coprocessor Interlocks:
 *    On MIPS I and MIPS II architectures (R3000 / R4000), coprocessor 1 (FPU) register
 *    transfers (`mtc1`) and comparison condition flags (`c.cond.fmt`) required software
 *    delay cycles before subsequent dependent instructions could safely read the result.
 *    The 1996 N64 SDK assembler always inserted software `nop` interlocks when in
 *    `.set reorder` mode.
 *
 *    Modern GAS is invoked with `-march=vr4300`. Because the physical NEC VR4300 CPU
 *    incorporates hardware interlock logic that stalls the pipeline on register hazards,
 *    modern GAS treats these software NOPs as redundant and omits them.
 *
 * The macros in this file override modern GAS synthetic instruction expansions and
 * reinstate the exact 1996 SGI/Cygnus assembler behavior, ensuring 100% byte-for-byte
 * bit-exact ROM reproduction.
 */

# Evaluate this file only once in case of recursive or multiple inclusions
.ifndef _GAS_COMPAT_INC_GUARD
.set _GAS_COMPAT_INC_GUARD, 1

/*
 * ==============================================================================
 * 1. Register Transfer: `move d, s`
 * ==============================================================================
 * PURPOSE:
 *   GCC 2.7.2 emits the `move $d, $s` pseudo-instruction for register assignment.
 *
 * HISTORICAL DIFFERENCE:
 *   - Modern GAS: Expands `move $d, $s` to `or $d, $s, $zero` (opcode 0x00000025).
 *   - 1996 SDK GAS: Always expanded `move $d, $s` to `addu $d, $s, $zero` (opcode 0x00000021).
 *
 * Without this macro, every single variable assignment in C compiled by GCC 2.7.2
 * causes a 4-byte bit mismatch against the retail ROM.
 */
.macro move d, s
    addu \d, \s, $0
.endm

/*
 * ==============================================================================
 * 2. Unsigned Remainder: `remu d, s, t` (C Modulo Operator `%`)
 * ==============================================================================
 * PURPOSE:
 *   When C code evaluates an unsigned modulo expression (`a % b`), GCC 2.7.2 emits
 *   the 3-operand synthetic instruction `remu $d, $s, $t`.
 *
 * HISTORICAL DIFFERENCE:
 *   The MIPS instruction set has no native single-instruction modulo operation;
 *   it uses the hardware division unit (`divu $s, $t`), placing the quotient in LO
 *   and remainder in HI.
 *
 *   - Modern GAS: Checks for zero divisor BEFORE issuing the division:
 *         bnez $t, .Lremu
 *         divu $0, $s, $t
 *         break 7
 *     .Lremu:
 *         mfhi $d
 *
 *   - 1996 SDK GAS: Issued `divu` FIRST, initiating the multi-cycle hardware division
 *     pipeline while simultaneously branching around the `break 7` trap in the branch
 *     delay slot:
 *         divu $0, $s, $t
 *         bnez $t, .Lremu
 *         nop
 *         break 7
 *     .Lremu:
 *         mfhi $d
 *         nop
 *         nop
 *
 *   Additionally, MIPS I/II hardware interlock rules required 2 cycles after `mfhi`
 *   before the destination register was valid, which the 1996 assembler emitted as
 *   two trailing NOPs.
 */
.macro remu d, s, t
    .set noreorder
    divu $0, \s, \t
    bnez \t, .Lremu_\@
    nop
    break 7
.Lremu_\@:
    mfhi \d
    nop
    nop
    .set reorder
.endm

/*
 * ==============================================================================
 * 3. Signed Integer Division: `div d, s, t` (C Division Operator `/`)
 * ==============================================================================
 * PURPOSE:
 *   When C code evaluates signed integer division (`a / b`), GCC 2.7.2 emits the
 *   3-operand synthetic instruction `div $d, $s, $t`.
 *
 * HISTORICAL DIFFERENCE:
 *   MIPS signed division must guard against two architectural fault conditions:
 *     1. Division by zero (`divisor == 0`): triggers `break 7`.
 *     2. 32-bit two's complement overflow (`-2147483648 / -1`): triggers `break 6`,
 *        since `0x80000000 / -1 = +2147483648`, which exceeds maximum positive s32.
 *
 *   1996 SDK GAS evaluated the zero check, overflow check, and emitted 2 trailing
 *   NOPs after `mflo $d` for MIPS II interlock compliance.
 *
 * SPECIAL HANDLING FOR DISASSEMBLED ASSEMBLY:
 *   Disassembled `.s` files produced by spimdisasm (e.g. `math.s`) frequently invoke
 *   native 2-operand or 3-operand hardware division with `$zero` as the destination
 *   (e.g. `div $zero, $v1, $v0`). The macro inspects `\d`; if it is `$zero` or `$0`,
 *   it passes through to the native hardware instruction without emitting synthetic checks.
 */
.macro define_div
    .macro div d, s, t
        .purgem div
        .ifc \d, $zero
            div \d, \s, \t
        .else
        .ifc \d, $0
            div \d, \s, \t
        .else
            .set noreorder
            .set noat
            div $0, \s, \t
            bnez \t, .Ldiv1_\@
            nop
            break 7
        .Ldiv1_\@:
            li $1, -1
            bne \t, $1, .Ldiv2_\@
            lui $1, 0x8000
            bne \s, $1, .Ldiv2_\@
            nop
            break 6
        .Ldiv2_\@:
            mflo \d
            nop
            nop
            .set at
            .set reorder
        .endif
        .endif
        define_div
    .endm
.endm
define_div

/*
 * ==============================================================================
 * 4. Coprocessor 1 (FPU) Software Interlocks: `mtc1` and `c.lt.d`
 * ==============================================================================
 * PURPOSE:
 *   When moving general-purpose registers to floating-point registers (`mtc1`) or
 *   performing double-precision floating-point comparisons (`c.lt.d`), MIPS I/II
 *   coprocessor pipeline hazards required software delay slots.
 *
 * HISTORICAL DIFFERENCE:
 *   - MIPS I/II Coprocessor 1 Hazard:
 *     After `mtc1 $s, $fd`, 1 instruction cycle must elapse before Coprocessor 1
 *     arithmetic operations (`cvt.d.w`, etc.) can access `$fd`.
 *     After `c.cond.fmt` (e.g. `c.lt.d`), 2 instruction cycles must elapse before
 *     the Coprocessor 1 condition flag can be tested by `bc1t` or `bc1f`.
 *
 *   - 1996 SDK GAS: Always inserted:
 *         1 NOP after `mtc1`
 *         2 NOPs after `c.cond.d`
 *
 *   - Modern GAS: Because `-march=vr4300` is targeted, modern GAS assumes the VR4300
 *     hardware interlock logic handles these stalls automatically, and omits the NOPs.
 *
 * GUARD:
 *   These macros are guarded by `__C_COMPILED__` (passed via `-Wa,--defsym=__C_COMPILED__=1`
 *   for C modules). Disassembled assembly files already have their explicit NOPs
 *   written into the disassembly by `spimdisasm`, so the macro must not duplicate them.
 */
.ifdef __C_COMPILED__
    .macro define_mtc1
        .macro mtc1 args:vararg
            .purgem mtc1
            mtc1 \args
            nop
            define_mtc1
        .endm
    .endm
    define_mtc1

    .macro define_c_lt_d
        .macro c.lt.d args:vararg
            .purgem c.lt.d
            c.lt.d \args
            nop
            nop
            define_c_lt_d
        .endm
    .endm
    define_c_lt_d
.endif

.endif /* _GAS_COMPAT_INC_GUARD */
